"""Galactic conquest from the single-player menu (MNU-34, FES-13, docs/ui/menu-screens.md).

Source contracts always run. The GPU case (`--eawr-front-end-test-campaign start`) needs the viewer
lane and the installed game: main menu -> single player -> custom campaign -> a row -> Start ->
the galactic mode ready with the chosen campaign entry.
"""

import json
import os
import pathlib
import re
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))

import test_front_end as front  # noqa: E402

SRC = ROOT / "apps/viewer/src"
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")

STARTED = re.compile(r"EAWR front end campaign started: (.+?); faction (.+?); difficulty (\w+)")
SUMMARY = re.compile(
    r"EAWR front end campaign (opened|selected): rows (\d+); factions (\d+); row (\S+); faction (.+?); "
    r"difficulty (\w+); start (\w+); options (\w+)")


class CampaignEntryContracts(unittest.TestCase):
    """The hand-off order and the data-driven lists, read from the sources."""

    def test_start_stops_the_demo_battle_before_the_launcher(self):
        source = (SRC / "front_end_mode.cpp").read_text(encoding="utf-8")
        start = source.split("void FrontEndMode::State::start_campaign(", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(start.index('stop_attract("campaign start")'), start.index("options.campaign_launcher("))
        self.assertIn("begin_campaign();", start)
        self.assertIn("show_notice(", start, "a refused start is told to the player")
        restore = source.split("std::string FrontEndMode::State::restore_saved(", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(restore.index('stop_attract("saved game restore")'), restore.index("options.saved_game_launcher("))

    def test_host_wires_the_campaign_launcher_to_the_galactic_mode(self):
        host = (SRC / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("front_options.campaign_launcher", host)
        self.assertIn("campaign_options.campaign = request.campaign;", host)
        self.assertIn("front_options.saved_game_launcher", host)

    def test_menu_campaign_uses_the_shared_galactic_command_line_parser(self):
        host = (SRC / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        for launcher in ("campaign_launcher", "saved_game_launcher"):
            body = host.split("front_options." + launcher + " =", 1)[1].split("\n            };", 1)[0]
            self.assertIn("GalacticMode::from_command_line()", body)
            self.assertIn("start_galactic(std::move(campaign_options))", body)

    def test_lists_come_from_the_data(self):
        screen = (SRC / "campaign_setup_screen.cpp").read_text(encoding="utf-8")
        self.assertIn("load_galactic_data(", screen)
        self.assertIn("tables_->campaigns", screen)
        self.assertIn("tables_->factions", screen)
        model = (ROOT / "src/presentation/ui/campaign_setup.cpp").read_text(encoding="utf-8")
        # No campaign, set or faction name is spelt in the model or the screen.
        for name in ("Sandbox", "Underworld", "Rebel", "Empire", "Consortium", "Zann"):
            self.assertNotIn(name, model)
            self.assertNotIn(name, screen)

    def test_screen_follows_the_staging_dialog(self):
        screen = (SRC / "campaign_setup_screen.cpp").read_text(encoding="utf-8")
        for control in ("IDC_STAGING_MAP_LIST", "IDC_FACTION_COMBO", "IDC_PLAY_BUTTON", "IDC_CANCEL_BUTTON",
                        "IDC_SELECT_OPTIONS", "IDC_RADIO_CUSTOM_EASY", "IDC_RADIO_CUSTOM_MEDIUM",
                        "IDC_RADIO_CUSTOM_HARD", "IDC_CAMPAIGN_DESCRIPTION", "IDC_SELECT_CUSTOM_MAPS"):
            self.assertIn(control, screen)
        self.assertIn("model::Placement::centre", screen)

    def test_story_campaigns_and_continue_stay_disabled(self):
        rules = (ROOT / "src/presentation/ui/front_end.cpp").read_text(encoding="utf-8")
        story = rules.split('"IDC_NEW_CAMPAIGN_BUTTON"', 1)[1].split("},", 1)[0]
        self.assertIn("GadgetKind::disabled", story)
        # The retail dialog has no Continue control: the shell adds none (continuing is Load Game).
        shell = (SRC / "front_end_mode.cpp").read_text(encoding="utf-8")
        self.assertNotIn("inject_continue", shell)


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the viewer GPU lane and installed game data")
class CampaignFromMenuGpu(unittest.TestCase):
    def test_custom_campaign_starts_the_galactic_mode(self):
        with tempfile.TemporaryDirectory(prefix="front-end-campaign-") as temporary:
            output = pathlib.Path(os.environ.get("EAWR_FRONT_END_EYECHECK", temporary)).resolve() / "campaign"
            output.mkdir(parents=True, exist_ok=True)
            report = output / "start.json"
            result, capture = front.run_front_end(output, "start", (
                "--eawr-front-end-test-campaign", "start", "--eawr-audio", "off", "--eawr-lighting", "sh",
                "--eawr-report", str(report), "--eawr-save-dir", str(pathlib.Path(temporary) / "saves")))
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertIn("EAWR front end ready", log)
            opened = SUMMARY.search(log)
            self.assertIsNotNone(opened, log[-6000:])
            self.assertEqual(opened.group(1), "opened")
            # WGP-40/66: campaigns come from the data, nothing is selected on open and Start waits.
            self.assertGreaterEqual(int(opened.group(2)), 1)
            self.assertGreaterEqual(int(opened.group(3)), 1)
            self.assertEqual((opened.group(4), opened.group(6), opened.group(7)), ("none", "normal", "disabled"))
            selected = [match for match in SUMMARY.finditer(log) if match.group(1) == "selected"]
            self.assertEqual(len(selected), 1, log[-6000:])
            self.assertEqual((selected[0].group(4), selected[0].group(6), selected[0].group(7)),
                             ("0", "normal", "enabled"))
            self.assertEqual(selected[0].group(8), "enabled")
            started = STARTED.search(log)
            self.assertIsNotNone(started, log[-6000:])
            self.assertIn("EAWR front end attract stopped: campaign start", log)
            self.assertLess(log.index("EAWR front end attract stopped: campaign start"),
                            log.index("EAWR front end campaign started:"))
            self.assertEqual(started.group(3), "normal")
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(data["mode"], "galactic")
            self.assertNotEqual(data["status"], "failed", data.get("failure"))
            self.assertEqual(data["failure"], "")
            self.assertEqual(data["campaign"], started.group(1))
            self.assertEqual(data["snapshot"]["ownership"], "authored-starting-forces")
            for label in ("single_player", "campaign_setup", "campaign_selected"):
                image = capture.parent / (capture.stem + "." + label + ".png")
                self.assertGreater(image.stat().st_size, 10000, label)


    def test_native_ai_switch_from_the_menu(self):
        # Exercise real menu selection and Start, without the all-AI capture override.
        output = pathlib.Path(tempfile.mkdtemp(prefix="front-end-native-ai-")).resolve()
        for flag, installed in ((None, True), ("off", False), ("on", True)):
            with self.subTest(flag=flag):
                label = flag or "default"
                report = output / (label + ".json")
                extra = ("--eawr-front-end-test-campaign", "start", "--eawr-attract", "off",
                         "--eawr-audio", "off", "--eawr-galactic-paused", "yes",
                         "--eawr-report", str(report), "--eawr-save-dir", str(output / label / "saves"))
                if flag is not None:
                    extra += ("--eawr-galactic-ai", flag)
                result, _ = front.run_front_end(output, label, extra)
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                self.assertIsNotNone(STARTED.search(result.stdout), result.stdout[-6000:])
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["mode"], "galactic")
                self.assertEqual(data["failure"], "")
                self.assertIs(data["live_session"]["native_ai_installed_at_start"], installed)
                self.assertEqual(bool(data["native_ai_players"]), installed)
                self.assertEqual("ai_activity" in data, installed)
                for row in data["native_ai_players"]:
                    self.assertTrue(row["faction"] and row["player_type"])
                if installed:
                    self.assertRegex(result.stdout, r"EAWR galactic: native AI on for [1-9][0-9]* AI players \(.+\)")
                else:
                    self.assertIn("EAWR galactic: native AI off", result.stdout)

    def test_galactic_reveal_switch_from_the_menu(self):
        # #3931: the draw-only fog reveal reaches a campaign started from the menu.
        output = pathlib.Path(tempfile.mkdtemp(prefix="front-end-galactic-reveal-")).resolve()
        for flag, revealed in ((None, False), ("off", False), ("on", True)):
            with self.subTest(flag=flag):
                label = flag or "default"
                report = output / (label + ".json")
                extra = ("--eawr-front-end-test-campaign", "start", "--eawr-attract", "off",
                         "--eawr-audio", "off", "--eawr-galactic-paused", "yes",
                         "--eawr-report", str(report), "--eawr-save-dir", str(output / label / "saves"))
                if flag is not None:
                    extra += ("--eawr-galactic-reveal", flag)
                result, _ = front.run_front_end(output, label, extra)
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                self.assertIsNotNone(STARTED.search(result.stdout), result.stdout[-6000:])
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["mode"], "galactic")
                self.assertEqual(data["failure"], "")
                self.assertIs(data["live_session"]["galactic_reveal"], revealed)


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the viewer GPU lane and installed game data")
class CampaignTechnologyGpu(unittest.TestCase):
    def test_accepted_level_five_for_every_faction_and_another_campaign(self):
        for campaign, internal, card, focus in (
                ("Sandbox_Equal_Footing_Rebel", 4, "R_Galactic_Turbolaser_Tower_Defenses", None),
                ("Sandbox_Equal_Footing_Empire", 5, "E_Galactic_Turbolaser_Tower_Defenses", None),
                ("Sandbox_Equal_Footing_Underworld", 5, "U_Ground_Vehicle_Factory", None),
                ("Sandbox_Rim_Worlds_Empire", 5, "Ground_Empire_Hypervelocity_Gun", 12)):
            with self.subTest(campaign=campaign):
                output = pathlib.Path(tempfile.mkdtemp(prefix="accepted-tech-five-")).resolve()
                report = output / "technology.json"
                extra = ("--eawr-front-end-test-campaign", "tech5:" + campaign,
                    "--eawr-audio", "off", "--eawr-lighting", "sh", "--eawr-environment", "map",
                    "--eawr-shadows", "on", "--eawr-report", str(report))
                if focus is not None:
                    extra += ("--eawr-galactic-focus", str(focus))
                result, capture = front.run_front_end(output, "technology", extra)
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                self.assertIn("EAWR campaign technology options: start=5 max=5 custom=1", result.stdout)
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["failure"], "")
                self.assertEqual(data["campaign"], campaign)
                self.assertEqual(data["technology"], {"player": data["local"]["player"],
                    "current": internal, "maximum": internal, "displayed": 5})
                label = next(row for row in data["command_bar"]["planet_block"] if row["name"] == "Text_Tech_Level")
                self.assertEqual(label["visible"], data["local"]["faction"] != "Underworld")
                if label["visible"]:
                    self.assertEqual(label["text"], "5")
                choice = next(row for row in data["command_bar"]["production_choices"] if row["type"] == card)
                self.assertTrue(choice["enabled"], choice)
                self.assertGreater(capture.stat().st_size, 10000)

    @classmethod
    def run_technology(cls):
        if hasattr(cls, "evidence"):
            return cls.evidence
        output = pathlib.Path(tempfile.mkdtemp(prefix="campaign-technology-")).resolve()
        report = output / "technology.json"
        result, capture = front.run_front_end(output, "technology", (
            "--eawr-front-end-test-campaign", "tech:Sandbox_Rim_Worlds_Empire", "--eawr-galactic-focus", "12",
            "--eawr-audio", "off", "--eawr-lighting", "sh",
            "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-report", str(report)))
        cls.evidence = result, capture, report
        return cls.evidence

    def test_starting_technology_setup_screen(self):
        result, capture, _ = self.run_technology()
        self.assertEqual(result.returncode, 0, result.stdout[-6000:])
        self.assertIn("EAWR campaign technology options: start=4 max=5 custom=1", result.stdout)
        self.assertIn("EAWR campaign unchanged technology edit: 1", result.stdout)
        image = capture.parent / (capture.stem + ".campaign_technology.png")
        self.assertGreater(image.stat().st_size, 10000)

    def test_level_four_build_list_in_game(self):
        result, capture, report = self.run_technology()
        self.assertEqual(result.returncode, 0, result.stdout[-6000:])
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["failure"], "")
        self.assertEqual(data["campaign"], "Sandbox_Rim_Worlds_Empire")
        # Stock Rim Worlds planet 12 is Fondor, with the authored heavy factory.
        # The hypervelocity gun requires tech 3 and that structure; startup at 4
        # permits its authored technology while preserving the prerequisite gate.
        choice = next(row for row in data["command_bar"]["production_choices"]
                      if row["type"] == "Ground_Empire_Hypervelocity_Gun")
        self.assertTrue(choice["enabled"], choice)
        self.assertEqual(choice["price"], 3000)
        self.assertGreater(capture.stat().st_size, 10000)


if __name__ == "__main__":
    unittest.main()
